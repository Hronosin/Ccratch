/*
 * Moves blocks with fake mouse input, saves, and checks the C that comes
 * out: the moves happened, nothing else changed, and it still compiles.
 *
 *   test_save SAVE.c OUTDIR
 *
 * Needs a window (text is measured with the GUI font): run under xvfb.
 */
#include "ccr_ast.h"
#include "ccr_codegen.h"
#include "ccr_editor.h"
#include "ccr_gui.h"
#include "ccr_lib.h"
#include "ccr_parse.h"
#include "ccr_paste.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static const char *g_input;
static const char *g_outdir;

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

static bool load(ccr_editor *ed)
{
    ccr_ast ast;
    ccr_ast_init(&ast);
    if (!ccr_parse_file(g_input, NULL, 0, &ast))
        return false;
    return ccr_editor_init(ed, &ast);
}

/* The statement whose original text starts with `prefix`. */
static ccr_node_id stmt(const ccr_editor *ed, const char *prefix)
{
    size_t n = strlen(prefix);
    for (ccr_node_id i = 0; i < ed->ast.node_count; ++i) {
        const ccr_node *k = N(ed, i);
        if (k->parent == CCR_NONE || N(ed, k->parent)->kind != CCR_NODE_BLOCK)
            continue;
        if (k->src_e - k->src_b >= n && memcmp(ed->ast.source + k->src_b, prefix, n) == 0)
            return i;
    }
    fprintf(stderr, "no statement starting with \"%s\"\n", prefix);
    ++g_failures;
    return CCR_NONE;
}

static void frame(ccr_editor *ed, ccr_vec2 m, bool pressed, bool down, bool released)
{
    ccr_editor_input in = { m, pressed, down, released, 1.0f, false, 0.0f };
    ccr_editor_update(ed, &in);
}

static void drag(ccr_editor *ed, ccr_vec2 from, ccr_vec2 to)
{
    frame(ed, from, true, true, false);
    for (int i = 1; i <= 6; ++i) {
        float t = (float)i / 6.0f;
        frame(ed, (ccr_vec2){ from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t },
              false, true, false);
    }
    frame(ed, to, false, false, true);
}

static ccr_vec2 corner(const ccr_editor *ed, ccr_node_id node)
{
    ccr_rect r = { 0 };
    if (!ccr_editor_node_rect(ed, node, &r)) {
        fprintf(stderr, "node %u is not on the canvas\n", node);
        ++g_failures;
    }
    return (ccr_vec2){ r.x + 6.0f, r.y + 6.0f };
}

/* World position of a snap point: after a statement, or first in a Block. */
static bool snap_at(const ccr_editor *ed, ccr_snap_kind kind, ccr_node_id ref, ccr_vec2 *out)
{
    for (uint32_t i = 0; i < ed->script_count; ++i)
        for (uint32_t k = 0; k < ed->scripts[i].blocks.snap_count; ++k) {
            const ccr_snap *s = &ed->scripts[i].blocks.snaps[k];
            if (s->kind == kind && s->ref == ref) {
                out->x = s->at.x + ed->scripts[i].pos.x + 6.0f;
                out->y = s->at.y + ed->scripts[i].pos.y + 6.0f;
                return true;
            }
        }
    fprintf(stderr, "no snap point for node %u\n", ref);
    ++g_failures;
    return false;
}

static ccr_vec2 far_away(float dy) { return (ccr_vec2){ 5000.0f, 3000.0f + dy }; }

/*
 * Scratch can only pick up a statement together with what is under it.
 * To take one out of the middle: lift the tail, put the rest back.
 */
static void lift_one(ccr_editor *ed, ccr_node_id s, float dy)
{
    ccr_node_id prev = CCR_NONE, block = N(ed, s)->parent;
    for (ccr_node_id c = N(ed, block)->first_child; c != s; c = N(ed, c)->next_sibling)
        prev = c;
    ccr_node_id rest = N(ed, s)->next_sibling;

    drag(ed, corner(ed, s), far_away(dy));
    if (rest == CCR_NONE)
        return;
    ccr_vec2 back;
    if (prev != CCR_NONE ? snap_at(ed, CCR_SNAP_AFTER, prev, &back)
                         : snap_at(ed, CCR_SNAP_FIRST, block, &back))
        drag(ed, corner(ed, rest), back);
}

static void put_after(ccr_editor *ed, ccr_node_id s, ccr_node_id after)
{
    ccr_vec2 at;
    if (snap_at(ed, CCR_SNAP_AFTER, after, &at))
        drag(ed, corner(ed, s), at);
}

static char *save_to(const ccr_editor *ed, const char *name, char *err, size_t cap)
{
    char *text = NULL;
    size_t len = 0;
    if (!ccr_codegen(&ed->ast, &text, &len, err, cap))
        return NULL;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_outdir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(text, 1, len, f);
        fclose(f);
    }
    return text;
}

/* The saved file compiles, and clang reads it back without errors. */
static void check_compiles(const char *name)
{
    char path[1024], cmd[2200];
    snprintf(path, sizeof path, "%s/%s", g_outdir, name);
    snprintf(cmd, sizeof cmd, "gcc -std=c11 -Wall -Werror -Wno-unused -fsyntax-only '%s'", path);
    int rc = system(cmd);
    if (rc != 127 << 8)   /* 127: no gcc here, skip */
        CHECK(rc == 0, "%s does not compile: %s", name, cmd);

    ccr_ast ast;
    ccr_ast_init(&ast);
    CHECK(ccr_parse_file(path, NULL, 0, &ast), "%s: clang could not read it", name);
    CHECK(ccr_ast_count_diags(&ast, CCR_DIAG_ERROR) == 0, "%s: clang finds errors", name);
    ccr_ast_free(&ast);
}

/* `a` comes before `b` in `text`, both present */
static void check_order(const char *text, const char *a, const char *b)
{
    const char *pa = strstr(text, a), *pb = strstr(text, b);
    CHECK(pa && pb && pa < pb, "expected \"%s\" before \"%s\"", a, b);
}

static int count(const char *text, const char *what)
{
    int n = 0;
    for (const char *p = strstr(text, what); p; p = strstr(p + 1, what))
        ++n;
    return n;
}

/* Every comment of the original is still there. */
static void check_comments_kept(const ccr_editor *ed, const char *text)
{
    CHECK(count(text, "//") == count(ed->ast.source, "//"), "lost a // comment");
    CHECK(count(text, "/*") == count(ed->ast.source, "/*"), "lost a /* comment");
}

/* ------------------------------------------------------------- tests */

static void test_unchanged(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    char err[256];
    char *text = save_to(&ed, "unchanged.c", err, sizeof err);
    CHECK(text && strcmp(text, ed.ast.source) == 0, "unchanged file did not come back as is");
    free(text);
    ccr_editor_free(&ed);
}

/* printf("hello") with the comment above it moves to just before return */
static void test_move_with_comment(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id hello = stmt(&ed, "printf(\"hello");
    ccr_node_id dowhile = stmt(&ed, "do b--;");
    lift_one(&ed, hello, 0);
    put_after(&ed, hello, dowhile);
    CHECK(ccr_editor_loose_count(&ed) == 0, "something was left loose");

    char err[256];
    char *text = save_to(&ed, "moved.c", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        check_order(text, "int a = 1, b = 2;", "// first: say hello");
        check_order(text, "do b--; while (b > 5);", "// first: say hello");
        check_order(text, "// first: say hello\n    printf(\"hello\\n\");", "return a + b");
        CHECK(strstr(text, "    if (n > 3)\n        n--;          // braceless then\n"
                           "    else if (n < 0)\n        n = 0;\n    else\n        n++;") != NULL,
              "the untouched if/else changed");
        CHECK(strstr(text, "/* group stays together */") != NULL, "lost the comment on the group");
        check_comments_kept(&ed, text);
        free(text);
        check_compiles("moved.c");
    }
    ccr_editor_free(&ed);
}

/* n++ leaves the braceless else, goes into the while loop */
static void test_braceless_body(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id inc = stmt(&ed, "n++");
    ccr_node_id bpp = stmt(&ed, "b++");
    drag(&ed, corner(&ed, inc), far_away(0));
    put_after(&ed, inc, bpp);

    char err[256];
    char *text = save_to(&ed, "braceless.c", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        CHECK(strstr(text, "    else {\n    }") != NULL, "empty else is not `else { }`");
        CHECK(strstr(text, "        b++;\n        n++;\n        // nothing else here\n    }") != NULL,
              "n++ not in the loop, or the loop's last comment moved");
        check_comments_kept(&ed, text);
        free(text);
        check_compiles("braceless.c");
    }
    ccr_editor_free(&ed);
}

/* a += i leaves the for body (8 spaces deep) for the function body (4) */
static void test_reindent(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id add = stmt(&ed, "a += i");
    ccr_node_id dowhile = stmt(&ed, "do b--;");
    drag(&ed, corner(&ed, add), far_away(0));
    put_after(&ed, add, dowhile);

    char err[256];
    char *text = save_to(&ed, "reindent.c", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        CHECK(strstr(text, "for (int i = 0; i < n; i++) {\n    }") != NULL, "empty for body");
        /* now 4 spaces deep, not 8. (It will not compile: i is out of scope
         * there. Saving still works; the editor then warns about the error.) */
        CHECK(strstr(text, "while (b > 5);\n    a += i;\n\n    return") != NULL,
              "a += i was not re-indented to the function body");
        free(text);
    }
    ccr_editor_free(&ed);
}

/* dragged out and left loose: gone from the file */
static void test_loose_is_left_out(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id bump = stmt(&ed, "counter += TWICE(n)");
    lift_one(&ed, bump, 0);
    CHECK(ccr_editor_loose_count(&ed) == 1, "expected one loose stack");

    char err[256];
    char *text = save_to(&ed, "loose.c", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        CHECK(strstr(text, "counter += TWICE(n)") == NULL, "the loose statement was saved");
        CHECK(strstr(text, "/* bump it */") == NULL, "its comment stayed behind");
        CHECK(strstr(text, "#define TWICE(x) ((x) * 2)") != NULL, "the macro is gone");
        free(text);
        check_compiles("loose.c");
    }
    ccr_editor_free(&ed);
}

/* a block with #ifdef between its statements is refused, not mangled */
static void test_refuses_directives(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id dec = stmt(&ed, "v--");
    ccr_node_id inc = stmt(&ed, "v++");
    lift_one(&ed, dec, 0);
    put_after(&ed, dec, inc);

    char err[256];
    char *text = save_to(&ed, "refused.c", err, sizeof err);
    CHECK(text == NULL, "saved a block that has #ifdef inside");
    CHECK(strstr(err, "#if") != NULL, "unhelpful reason: %s", err);
    free(text);
    ccr_editor_free(&ed);
}

/* ------------------------------------------------------------- undo */

static char *generate(const ccr_editor *ed)
{
    char *text = NULL, err[256];
    size_t len = 0;
    if (!ccr_codegen(&ed->ast, &text, &len, err, sizeof err)) {
        fprintf(stderr, "codegen failed: %s\n", err);
        ++g_failures;
        return NULL;
    }
    return text;
}

static ccr_placed *placement(const ccr_editor *ed, uint32_t *n)
{
    ccr_placed *v = malloc((ed->script_count + 1) * sizeof *v);
    for (uint32_t i = 0; i < ed->script_count; ++i)
        v[i] = (ccr_placed){ ed->scripts[i].root, ed->scripts[i].pos };
    *n = ed->script_count;
    return v;
}

/* the same scripts in the same places (order may differ: drawing order) */
static bool same_canvas(const ccr_editor *ed, const ccr_placed *v, uint32_t n)
{
    if (ed->script_count != n)
        return false;
    for (uint32_t i = 0; i < n; ++i) {
        bool found = false;
        for (uint32_t k = 0; k < ed->script_count && !found; ++k)
            found = ed->scripts[k].root == v[i].root &&
                    ed->scripts[k].pos.x == v[i].pos.x && ed->scripts[k].pos.y == v[i].pos.y;
        if (!found)
            return false;
    }
    return true;
}

/* many edits, all undone: the file and the canvas are exactly as loaded */
static void test_undo_everything(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    uint32_t n0;
    ccr_placed *canvas0 = placement(&ed, &n0);

    lift_one(&ed, stmt(&ed, "printf(\"hello"), 0);
    put_after(&ed, stmt(&ed, "printf(\"hello"), stmt(&ed, "do b--;"));
    ccr_node_id inc = stmt(&ed, "n++");
    drag(&ed, corner(&ed, inc), far_away(0));
    put_after(&ed, inc, stmt(&ed, "b++"));
    lift_one(&ed, stmt(&ed, "counter += TWICE(n)"), 400);   /* stays loose */
    ccr_node_id work = 0;
    for (ccr_node_id i = 0; i < ed.ast.node_count; ++i)
        if (ed.ast.nodes[i].kind == CCR_NODE_FUNCTION &&
            strcmp(ccr_ast_str(&ed.ast, ed.ast.nodes[i].name), "guarded") == 0 &&
            (ed.ast.nodes[i].flags & CCR_FLAG_DEFINITION))
            work = i;
    ccr_vec2 at = corner(&ed, work);
    drag(&ed, at, (ccr_vec2){ at.x + 300.0f, at.y + 90.0f });   /* move a whole function */

    CHECK(ccr_editor_modified(&ed), "edits did not count as a change");
    CHECK(ccr_editor_loose_count(&ed) == 1, "expected one loose stack before undo");
    char *edited = generate(&ed);
    uint32_t n1;
    ccr_placed *canvas1 = placement(&ed, &n1);

    uint32_t steps = 0;
    while (ccr_editor_undo(&ed))
        ++steps;
    CHECK(steps == ed.history_count, "undid %u of %u steps", steps, ed.history_count);
    CHECK(steps >= 7, "only %u undo steps were recorded", steps);
    char *back = generate(&ed);
    CHECK(back && strcmp(back, ed.ast.source) == 0, "undoing everything did not restore the file");
    CHECK(same_canvas(&ed, canvas0, n0), "undoing everything did not restore the canvas");
    CHECK(!ccr_editor_modified(&ed), "fully undone, yet marked as changed");
    CHECK(ccr_editor_loose_count(&ed) == 0, "a loose stack survived undo");

    while (ccr_editor_redo(&ed))
        ;
    char *again = generate(&ed);
    CHECK(again && edited && strcmp(again, edited) == 0, "redoing everything gave a different file");
    CHECK(same_canvas(&ed, canvas1, n1), "redoing everything gave a different canvas");
    CHECK(ccr_editor_loose_count(&ed) == 1, "the loose stack did not come back on redo");

    free(edited);
    free(back);
    free(again);
    free(canvas0);
    free(canvas1);
    ccr_editor_free(&ed);
}

/* the saved point: undo past it is a change, redo back to it is not */
static void test_undo_and_save_point(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id inc = stmt(&ed, "n++");
    drag(&ed, corner(&ed, inc), far_away(0));
    CHECK(ccr_editor_modified(&ed), "move not counted");
    ccr_editor_mark_saved(&ed);
    CHECK(!ccr_editor_modified(&ed), "saved, still marked as changed");
    CHECK(ccr_editor_undo(&ed), "nothing to undo");
    CHECK(ccr_editor_modified(&ed), "undo past the save is not a change?");
    CHECK(ccr_editor_redo(&ed), "nothing to redo");
    CHECK(!ccr_editor_modified(&ed), "back at the saved state, still marked as changed");

    /* undo, then a new edit: redo is gone, and so is the saved state */
    CHECK(ccr_editor_undo(&ed), "nothing to undo");
    ccr_node_id dec = stmt(&ed, "n--");
    drag(&ed, corner(&ed, dec), far_away(0));
    CHECK(!ccr_editor_redo(&ed), "redo survived a new edit");
    CHECK(ccr_editor_modified(&ed), "a different edit looks saved");

    /* a click is not a step */
    uint32_t steps = ed.history_len;
    ccr_vec2 c = corner(&ed, stmt(&ed, "b++"));
    frame(&ed, c, true, true, false);
    frame(&ed, c, false, false, true);
    CHECK(ed.history_len == steps, "a click was recorded as an undo step");
    ccr_editor_free(&ed);
}

/* ---------------------------------------------------------- palette */

/* Takes a block from the palette and drops it so its top-left lands on
 * `target` (a snap point from snap_at, which adds the 6-unit grip). */
static ccr_node_id spawn_at(ccr_editor *ed, ccr_template t, ccr_vec2 target)
{
    ccr_vec2 start = far_away(-900.0f);
    CHECK(ccr_editor_spawn(ed, t, start), "could not take %s from the palette",
          ccr_template_label(t));
    ccr_node_id loose = ed->scripts[ed->script_count - 1].root;
    ccr_node_id made = ed->ast.nodes[loose].first_child;
    /* spawn holds the block 16 units in from its corner */
    ccr_vec2 drop = { target.x - 6.0f + 16.0f, target.y - 6.0f + 16.0f };
    for (int i = 1; i <= 6; ++i) {
        float k = (float)i / 6.0f;
        frame(ed, (ccr_vec2){ start.x + (drop.x - start.x) * k, start.y + (drop.y - start.y) * k },
              false, true, false);
    }
    frame(ed, drop, false, false, true);
    return made;
}

static ccr_node_id kid(const ccr_editor *ed, ccr_node_id id, int index)
{
    ccr_node_id c = N(ed, id)->first_child;
    while (c != CCR_NONE && index-- > 0)
        c = N(ed, c)->next_sibling;
    return c;
}

/* new if + code line, an old statement moved in; new for loop */
static void test_palette_blocks(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_vec2 at;

    /* `if` after the struct declaration, condition typed in */
    snap_at(&ed, CCR_SNAP_AFTER, stmt(&ed, "struct pair"), &at);
    ccr_node_id iff = spawn_at(&ed, CCR_TEMPLATE_IF, at);
    CHECK(N(&ed, iff)->kind == CCR_NODE_IF, "the palette gave something else");
    CHECK(ccr_editor_loose_count(&ed) == 0, "the new if did not snap in");
    ccr_rect empty_slot, typed_slot, undone_slot;
    ccr_editor_node_rect(&ed, kid(&ed, iff, 0), &empty_slot);
    CHECK(ccr_editor_set_text(&ed, kid(&ed, iff, 0), "n > 100"), "could not type the condition");
    ccr_editor_node_rect(&ed, kid(&ed, iff, 0), &typed_slot);
    CHECK(typed_slot.w > empty_slot.w + 40.0f, "the field did not grow with its text (%.0f -> %.0f)",
          empty_slot.w, typed_slot.w);
    ccr_editor_undo(&ed);
    ccr_editor_node_rect(&ed, kid(&ed, iff, 0), &undone_slot);
    CHECK(undone_slot.w == empty_slot.w, "undo did not shrink the field back");
    ccr_editor_redo(&ed);

    /* printf("hello") with its comment goes inside it */
    ccr_node_id hello = stmt(&ed, "printf(\"hello");
    lift_one(&ed, hello, 0);
    ccr_node_id then = kid(&ed, iff, 1);
    if (snap_at(&ed, CCR_SNAP_FIRST, then, &at))
        drag(&ed, corner(&ed, hello), at);
    CHECK(N(&ed, hello)->parent == then, "printf did not go into the new if");

    /* and a code line under it */
    snap_at(&ed, CCR_SNAP_AFTER, hello, &at);
    ccr_node_id line = spawn_at(&ed, CCR_TEMPLATE_CODE, at);
    CHECK(N(&ed, line)->parent == then, "the code line did not go into the if");
    ccr_editor_set_text(&ed, line, "n = 100");

    /* a for loop before return, with a line inside */
    snap_at(&ed, CCR_SNAP_AFTER, stmt(&ed, "do b--;"), &at);
    ccr_node_id loop = spawn_at(&ed, CCR_TEMPLATE_FOR, at);
    snap_at(&ed, CCR_SNAP_FIRST, kid(&ed, loop, 3), &at);
    ccr_node_id inner = spawn_at(&ed, CCR_TEMPLATE_CODE, at);
    ccr_editor_set_text(&ed, inner, "b += i");
    CHECK(ccr_editor_loose_count(&ed) == 0, "something was left loose");

    char err[256];
    char *text = save_to(&ed, "palette.c", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        CHECK(strstr(text, "    struct pair { int x; int y; } pr = { a, b };\n"
                           "    if (n > 100) {\n"
                           "        // first: say hello\n"
                           "        printf(\"hello\\n\");\n"
                           "        n = 100;\n"
                           "    }\n"
                           "\n"
                           "    /* bump it */\n") != NULL, "the new if is not as expected");
        CHECK(strstr(text, "    do b--; while (b > 5);\n"
                           "    for (int i = 0; i < n; i++) {\n"
                           "        b += i;\n"
                           "    }\n"
                           "\n"
                           "    return a + b") != NULL, "the new for is not as expected");
        check_comments_kept(&ed, text);
        free(text);
        check_compiles("palette.c");
    }

    /* all of it undone: the file as it was */
    while (ccr_editor_undo(&ed))
        ;
    char *back = generate(&ed);
    CHECK(back && strcmp(back, ed.ast.source) == 0, "undoing palette work did not restore the file");
    free(back);
    ccr_editor_free(&ed);
}

/* every palette block prints as valid C */
static void test_every_template(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    static const char *const fill[CCR_TEMPLATE_COUNT] = {
        [CCR_TEMPLATE_CODE] = "n++", [CCR_TEMPLATE_IF] = "n > 0", [CCR_TEMPLATE_IF_ELSE] = "n > 0",
        [CCR_TEMPLATE_WHILE] = "n > 0", [CCR_TEMPLATE_DO_WHILE] = "n > 0", [CCR_TEMPLATE_RETURN] = "n",
    };
    ccr_node_id after = stmt(&ed, "do b--;");
    for (int t = 0; t <= CCR_TEMPLATE_CONTINUE; ++t) {   /* the first ones; the rest below */
        if (t == CCR_TEMPLATE_BREAK || t == CCR_TEMPLATE_CONTINUE)
            continue;   /* need a loop around them; covered inside the while below */
        if (!(ccr_template_langs((ccr_template)t) & CCR_TLANG_C))
            continue;   /* C++'s: this file is C */
        ccr_vec2 at;
        snap_at(&ed, CCR_SNAP_AFTER, after, &at);
        ccr_node_id made = spawn_at(&ed, (ccr_template)t, at);
        CHECK(N(&ed, made)->parent == N(&ed, after)->parent, "%s did not snap",
              ccr_template_label((ccr_template)t));
        ccr_node_id slot = t == CCR_TEMPLATE_CODE ? made
                         : t == CCR_TEMPLATE_DO_WHILE ? kid(&ed, made, 1)
                         : t == CCR_TEMPLATE_FOR ? CCR_NONE : kid(&ed, made, 0);
        if (slot != CCR_NONE && fill[t])
            ccr_editor_set_text(&ed, slot, fill[t]);
        if (t == CCR_TEMPLATE_WHILE) {   /* break and continue inside it */
            ccr_vec2 in;
            snap_at(&ed, CCR_SNAP_FIRST, kid(&ed, made, 1), &in);
            ccr_node_id brk = spawn_at(&ed, CCR_TEMPLATE_BREAK, in);
            snap_at(&ed, CCR_SNAP_AFTER, brk, &in);
            spawn_at(&ed, CCR_TEMPLATE_CONTINUE, in);
        }
        if (t != CCR_TEMPLATE_RETURN)
            after = made;   /* keep the return last, the rest would be dead code */
    }

    char err[256];
    char *text = save_to(&ed, "templates.c", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        CHECK(strstr(text, "if (n > 0) {\n    }") && strstr(text, "} else {\n    }") &&
              strstr(text, "while (n > 0) {\n        break;\n        continue;\n    }") &&
              strstr(text, "do {\n    } while (n > 0);") && strstr(text, "return n;") &&
              strstr(text, "n++;"), "a template printed wrong");
        free(text);
        check_compiles("templates.c");
    }
    ccr_editor_free(&ed);
}

/* --------------------------------------------- operators, variables... */

/* A new block's name or type field. */
static ccr_node_id word(const ccr_editor *ed, ccr_node_id block, uint32_t which)
{
    for (ccr_node_id c = N(ed, block)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling)
        if (N(ed, c)->flags & which)
            return c;
    fprintf(stderr, "node %u has no such field\n", block);
    ++g_failures;
    return CCR_NONE;
}

/* The index-th slot of a new block (name and type fields not counted). */
static ccr_node_id slot_of(const ccr_editor *ed, ccr_node_id block, int index)
{
    for (ccr_node_id c = N(ed, block)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling)
        if (!(N(ed, c)->flags & CCR_FLAG_WORD) && index-- == 0)
            return c;
    fprintf(stderr, "node %u has no such slot\n", block);
    ++g_failures;
    return CCR_NONE;
}

/* A statement from the palette, dropped after `after`. */
static ccr_node_id add_after(ccr_editor *ed, ccr_template t, ccr_node_id after)
{
    ccr_vec2 at = { 0, 0 };
    snap_at(ed, CCR_SNAP_AFTER, after, &at);
    ccr_node_id made = spawn_at(ed, t, at);
    CHECK(N(ed, made)->parent == N(ed, after)->parent, "%s did not snap after node %u",
          ccr_template_label(t), after);
    return made;
}

/* The first statement inside `block` (a body). */
static ccr_node_id add_first(ccr_editor *ed, ccr_template t, ccr_node_id block)
{
    ccr_vec2 at = { 0, 0 };
    snap_at(ed, CCR_SNAP_FIRST, block, &at);
    ccr_node_id made = spawn_at(ed, t, at);
    CHECK(N(ed, made)->parent == block, "%s did not go into node %u", ccr_template_label(t), block);
    return made;
}

/* A reporter from the palette, dropped into an empty slot. */
static ccr_node_id fill(ccr_editor *ed, ccr_template t, ccr_node_id hole)
{
    const ccr_node_id parent = N(ed, hole)->parent;
    ccr_vec2 at = { 0, 0 };
    snap_at(ed, CCR_SNAP_SLOT, hole, &at);
    ccr_node_id made = spawn_at(ed, t, at);
    CHECK(N(ed, made)->parent == parent, "%s did not drop into the slot",
          ccr_template_label(t));
    return made;
}

static void type(ccr_editor *ed, ccr_node_id field, const char *text)
{
    CHECK(ccr_editor_set_text(ed, field, text), "could not type \"%s\"", text);
}

/* Scratch-style C: declarations, operators nested in operators, pointers,
 * switch, goto, casts; parentheses only where C needs them. */
static void test_new_blocks(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id at = stmt(&ed, "do b--;");

    /* int total = n * 2 + 1;  (no parentheses: * binds tighter) */
    ccr_node_id total = add_after(&ed, CCR_TEMPLATE_DECL_INIT, at);
    type(&ed, word(&ed, total, CCR_FLAG_WORD_NAME), "total");
    ccr_node_id sum = fill(&ed, CCR_TEMPLATE_ADD, slot_of(&ed, total, 0));
    ccr_node_id prod = fill(&ed, CCR_TEMPLATE_MUL, slot_of(&ed, sum, 0));
    type(&ed, slot_of(&ed, prod, 0), "n");
    type(&ed, slot_of(&ed, prod, 1), "2");
    type(&ed, slot_of(&ed, sum, 1), "1");

    /* total = total - (n + b);  (parentheses: they are needed) */
    ccr_node_id set = add_after(&ed, CCR_TEMPLATE_ASSIGN, total);
    type(&ed, slot_of(&ed, set, 0), "total");
    ccr_node_id diff = fill(&ed, CCR_TEMPLATE_SUB, slot_of(&ed, set, 1));
    type(&ed, slot_of(&ed, diff, 0), "total");
    ccr_node_id inner = fill(&ed, CCR_TEMPLATE_ADD, slot_of(&ed, diff, 1));
    type(&ed, slot_of(&ed, inner, 0), "n");
    type(&ed, slot_of(&ed, inner, 1), "b");

    /* int *p = &total;  *p += 1; */
    ccr_node_id ptr = add_after(&ed, CCR_TEMPLATE_DECL_INIT, set);
    type(&ed, word(&ed, ptr, CCR_FLAG_WORD_TYPE), "int *");
    type(&ed, word(&ed, ptr, CCR_FLAG_WORD_NAME), "p");
    type(&ed, slot_of(&ed, fill(&ed, CCR_TEMPLATE_ADDR, slot_of(&ed, ptr, 0)), 0), "total");
    ccr_node_id bump = add_after(&ed, CCR_TEMPLATE_ADD_ASSIGN, ptr);
    type(&ed, slot_of(&ed, fill(&ed, CCR_TEMPLATE_DEREF, slot_of(&ed, bump, 0)), 0), "p");
    type(&ed, slot_of(&ed, bump, 1), "1");

    /* int size = (int)sizeof(long); */
    ccr_node_id size = add_after(&ed, CCR_TEMPLATE_DECL_INIT, bump);
    type(&ed, word(&ed, size, CCR_FLAG_WORD_NAME), "size");
    ccr_node_id cast = fill(&ed, CCR_TEMPLATE_CAST, slot_of(&ed, size, 0));
    ccr_node_id szof = fill(&ed, CCR_TEMPLATE_SIZEOF_TYPE, slot_of(&ed, cast, 0));
    type(&ed, word(&ed, szof, CCR_FLAG_WORD_TYPE), "long");

    /* int arr[3] = { 0 };  arr[1] = pr.y;  total = n > 0 ? total : -total; */
    ccr_node_id arr = add_after(&ed, CCR_TEMPLATE_DECL_INIT, size);
    type(&ed, word(&ed, arr, CCR_FLAG_WORD_NAME), "arr[3]");
    type(&ed, slot_of(&ed, arr, 0), "{ 0 }");
    ccr_node_id put = add_after(&ed, CCR_TEMPLATE_ASSIGN, arr);
    ccr_node_id idx = fill(&ed, CCR_TEMPLATE_INDEX, slot_of(&ed, put, 0));
    type(&ed, slot_of(&ed, idx, 0), "arr");
    type(&ed, slot_of(&ed, idx, 1), "1");
    ccr_node_id mem = fill(&ed, CCR_TEMPLATE_MEMBER, slot_of(&ed, put, 1));
    type(&ed, slot_of(&ed, mem, 0), "pr");
    type(&ed, word(&ed, mem, CCR_FLAG_WORD_NAME), "y");
    ccr_node_id pick = add_after(&ed, CCR_TEMPLATE_ASSIGN, put);
    type(&ed, slot_of(&ed, pick, 0), "total");
    ccr_node_id tern = fill(&ed, CCR_TEMPLATE_TERNARY, slot_of(&ed, pick, 1));
    type(&ed, slot_of(&ed, tern, 0), "n > 0");
    type(&ed, slot_of(&ed, tern, 1), "total");
    type(&ed, slot_of(&ed, tern, 2), "-total");

    /* if (!(n == 3)) goto skip;  ...  skip: */
    ccr_node_id iff = add_after(&ed, CCR_TEMPLATE_IF, pick);
    ccr_node_id eq = fill(&ed, CCR_TEMPLATE_EQ, slot_of(&ed, fill(&ed, CCR_TEMPLATE_NOT,
                                                                   slot_of(&ed, iff, 0)), 0));
    type(&ed, slot_of(&ed, eq, 0), "n");
    type(&ed, slot_of(&ed, eq, 1), "3");
    ccr_node_id go = add_first(&ed, CCR_TEMPLATE_GOTO, slot_of(&ed, iff, 1));
    type(&ed, word(&ed, go, CCR_FLAG_WORD_NAME), "skip");

    /* switch (n) { case 1: { total++; break; } default: { total--; } } */
    ccr_node_id sw = add_after(&ed, CCR_TEMPLATE_SWITCH, iff);
    type(&ed, slot_of(&ed, sw, 0), "n");
    ccr_node_id one = add_first(&ed, CCR_TEMPLATE_CASE, slot_of(&ed, sw, 1));
    type(&ed, slot_of(&ed, one, 0), "1");
    ccr_node_id inc = add_first(&ed, CCR_TEMPLATE_INC, slot_of(&ed, one, 1));
    type(&ed, slot_of(&ed, inc, 0), "total");
    add_after(&ed, CCR_TEMPLATE_BREAK, inc);
    ccr_node_id dflt = add_after(&ed, CCR_TEMPLATE_DEFAULT, one);
    type(&ed, slot_of(&ed, add_first(&ed, CCR_TEMPLATE_DEC, slot_of(&ed, dflt, 0)), 0), "total");

    ccr_node_id label = add_after(&ed, CCR_TEMPLATE_LABEL, sw);
    type(&ed, word(&ed, label, CCR_FLAG_WORD_NAME), "skip");
    ccr_node_id lbl;
    CHECK(ccr_editor_jump(&ed, go, &lbl) == CCR_JUMP_OK && lbl == label,
          "the new goto does not find its new label");

    /* printf("%d %d\n", total, arr[1] + size); */
    ccr_node_id say = add_after(&ed, CCR_TEMPLATE_CALL_STMT, label);
    type(&ed, word(&ed, say, CCR_FLAG_WORD_NAME), "printf");
    type(&ed, slot_of(&ed, say, 0), "\"%d %d\\n\", total, arr[1] + size");
    add_after(&ed, CCR_TEMPLATE_PRINTF, say);
    CHECK(ccr_editor_loose_count(&ed) == 0, "something was left loose");

    char err[256];
    char *text = save_to(&ed, "new_blocks.c", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        static const char *const want[] = {
            "    int total = n * 2 + 1;\n",
            "    total = total - (n + b);\n",
            "    int *p = &total;\n",
            "    *p += 1;\n",
            "    int size = (int)sizeof(long);\n",
            "    int arr[3] = { 0 };\n",
            "    arr[1] = pr.y;\n",
            "    total = n > 0 ? total : -total;\n",
            "    if (!(n == 3)) {\n        goto skip;\n    }\n",
            "    switch (n) {\n        case 1: {\n            total++;\n            break;\n        }\n"
            "        default: {\n            total--;\n        }\n    }\n",
            "    skip: ;\n",
            "    printf(\"%d %d\\n\", total, arr[1] + size);\n",
            "    printf(\"Hello, world!\\n\");\n",
        };
        for (size_t i = 0; i < sizeof want / sizeof want[0]; ++i)
            CHECK(strstr(text, want[i]) != NULL, "missing:\n%s", want[i]);
        free(text);
        check_compiles("new_blocks.c");
    }

    /* a name left empty is not saved as broken C */
    type(&ed, word(&ed, total, CCR_FLAG_WORD_NAME), "");
    text = save_to(&ed, "new_blocks_empty.c", err, sizeof err);
    CHECK(text == NULL, "a declaration without a name was saved");
    free(text);

    /* and all of it undoes, back to the file as it was */
    while (ccr_editor_undo(&ed))
        ;
    char *back = generate(&ed);
    CHECK(back && strcmp(back, ed.ast.source) == 0, "undoing the new blocks did not restore the file");
    free(back);
    ccr_editor_free(&ed);
}

/* Every template can be taken from the palette, and lies there loose. */
static void test_all_templates_spawn(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    for (int t = 0; t < CCR_TEMPLATE_COUNT; ++t) {
        const ccr_vec2 where = { 5000.0f + (float)t * 600.0f, -4000.0f };   /* apart: no snapping */
        CHECK(ccr_editor_spawn(&ed, (ccr_template)t, where),
              "%s: spawn refused", ccr_template_label((ccr_template)t));
        frame(&ed, where, false, false, true);
        const ccr_node_id root = ed.scripts[ed.script_count - 1].root;
        CHECK(((N(&ed, root)->flags & CCR_FLAG_REPORTER) != 0) ==
              ccr_template_is_reporter((ccr_template)t), "%s: reporter or not?",
              ccr_template_label((ccr_template)t));
        CHECK(ed.scripts[ed.script_count - 1].blocks.bounds.w > 10.0f, "%s: drawn as nothing",
              ccr_template_label((ccr_template)t));
    }
    int toplevel = 0;
    for (int t = 0; t < CCR_TEMPLATE_COUNT; ++t)
        toplevel += ccr_template_is_toplevel((ccr_template)t);
    /* declarations for the file join it; everything else lies loose */
    CHECK(ccr_editor_loose_count(&ed) == (uint32_t)(CCR_TEMPLATE_COUNT - toplevel),
          "%u loose, expected %d", ccr_editor_loose_count(&ed), CCR_TEMPLATE_COUNT - toplevel);
    ccr_editor_free(&ed);
}

/* thrown onto the palette: gone; from the code too */
static void test_trash(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_vec2 start = far_away(0);
    ccr_editor_spawn(&ed, CCR_TEMPLATE_WHILE, start);
    ccr_editor_input in = { start, false, false, true, 1.0f, true, 0.0f };
    ccr_editor_update(&ed, &in);
    CHECK(ccr_editor_loose_count(&ed) == 0, "a block dropped on the palette stayed");

    ccr_node_id bump = stmt(&ed, "counter += TWICE(n)");
    lift_one(&ed, bump, 0);
    ccr_vec2 c = corner(&ed, bump);
    frame(&ed, c, true, true, false);
    frame(&ed, (ccr_vec2){ c.x + 50.0f, c.y }, false, true, false);
    ccr_editor_input drop = { { c.x + 60.0f, c.y }, false, false, true, 1.0f, true, 0.0f };
    ccr_editor_update(&ed, &drop);
    CHECK(ccr_editor_loose_count(&ed) == 0, "the thrown-away statement is still loose");

    char err[256];
    char *text = save_to(&ed, "trash.c", err, sizeof err);
    CHECK(text && !strstr(text, "counter += TWICE"), "the deleted statement was saved");
    free(text);
    CHECK(ccr_editor_undo(&ed) && ccr_editor_loose_count(&ed) == 1, "undo did not bring it back");
    ccr_editor_free(&ed);
}

/* The pie menu's actions: delete, duplicate, comment out. */
static void test_delete(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id bump = stmt(&ed, "counter += TWICE(n)");
    ccr_context c = ccr_editor_context(&ed, corner(&ed, bump));
    CHECK(c.item == bump && c.can_delete && c.can_duplicate && c.can_comment,
          "the context under a statement is wrong");
    CHECK(ccr_editor_delete(&ed, bump), "delete refused");

    char err[256];
    char *text = save_to(&ed, "deleted.c", err, sizeof err);
    CHECK(text && !strstr(text, "counter += TWICE") && !strstr(text, "bump it"),
          "the deleted statement (or its comment) was saved");
    CHECK(text && strstr(text, "struct pair") && strstr(text, "if (n > 3)"),
          "its neighbours went with it");
    free(text);
    check_compiles("deleted.c");

    CHECK(ccr_editor_undo(&ed), "undo of delete failed");
    text = generate(&ed);
    CHECK(text && strcmp(text, ed.ast.source) == 0, "undoing a delete did not restore the file");
    free(text);
    ccr_editor_free(&ed);
}

static void test_duplicate(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id hello = stmt(&ed, "printf(\"hello\\n\")");
    ccr_vec2 m = corner(&ed, hello);
    CHECK(ccr_editor_duplicate(&ed, hello, m), "duplicate refused");
    CHECK(ccr_editor_loose_count(&ed) == 1, "the copy is not on the canvas");

    /* carried without holding a button, it lands on the next click */
    ccr_vec2 at;
    if (snap_at(&ed, CCR_SNAP_AFTER, hello, &at)) {
        ccr_vec2 hold = { at.x + 10.0f, at.y + 10.0f };
        frame(&ed, (ccr_vec2){ m.x + 40.0f, m.y + 40.0f }, false, false, false);
        CHECK(ed.drag == CCR_DRAG_MOVING, "the copy was dropped before the click");
        frame(&ed, hold, false, false, false);
        frame(&ed, hold, true, true, false);
        frame(&ed, hold, false, false, true);
    }
    CHECK(ed.drag == CCR_DRAG_IDLE && ccr_editor_loose_count(&ed) == 0,
          "the copy did not snap in");

    char err[256];
    char *text = save_to(&ed, "duplicated.c", err, sizeof err);
    CHECK(text && count(text, "printf(\"hello\\n\");") == 2, "expected two hellos");
    CHECK(text && count(text, "first: say hello") == 2, "the comment did not come with the copy");
    free(text);
    check_compiles("duplicated.c");

    CHECK(ccr_editor_undo(&ed), "undo of duplicate failed");
    text = generate(&ed);
    CHECK(text && strcmp(text, ed.ast.source) == 0, "duplicate took more than one undo");
    free(text);
    CHECK(ccr_editor_loose_count(&ed) == 0, "undo left the copy lying around");
    ccr_editor_free(&ed);
}

static void test_comment_out(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id loop = stmt(&ed, "while (b < 10)");
    ccr_node_id bump = stmt(&ed, "counter += TWICE(n)");
    CHECK(ccr_editor_toggle_comment(&ed, loop), "comment out refused");
    CHECK(ccr_editor_toggle_comment(&ed, bump), "comment out refused");
    CHECK(ccr_editor_context(&ed, corner(&ed, loop)).commented, "the context does not see it");

    char err[256];
    char *text = save_to(&ed, "commented.c", err, sizeof err);
    CHECK(text && strstr(text, "    // while (b < 10) {\n    //     b++;\n"
                               "    //     // nothing else here\n    // }\n"),
          "the loop was not commented out line by line");
    CHECK(text && strstr(text, "    // /* bump it */\n    // counter += TWICE(n);\n"),
          "the statement and its comment were not commented out");
    CHECK(text && strstr(text, "    do b--;"), "the next statement was commented too");
    free(text);
    check_compiles("commented.c");

    /* moved one level deeper while commented: the slashes follow it */
    ccr_node_id hello = stmt(&ed, "printf(\"hello\\n\")");
    ccr_node_id inner = stmt(&ed, "b++");
    lift_one(&ed, hello, 0);
    CHECK(ccr_editor_toggle_comment(&ed, hello), "comment out refused");
    put_after(&ed, hello, inner);
    text = save_to(&ed, "commented_moved.c", err, sizeof err);
    CHECK(text && strstr(text, "    //     // // first: say hello\n    //     // printf(\"hello\\n\");\n"),
          "a commented statement moved into the commented loop is wrong");
    free(text);
    check_compiles("commented_moved.c");
    for (int i = 0; i < 4; ++i)   /* lift, put back, comment, move */
        CHECK(ccr_editor_undo(&ed), "undo %d failed", i);

    /* back on: exactly as it was */
    CHECK(ccr_editor_toggle_comment(&ed, loop) && ccr_editor_toggle_comment(&ed, bump),
          "uncomment refused");
    text = generate(&ed);
    CHECK(text && strstr(text, "    while (b < 10) {\n        b++;"), "uncommented loop is wrong");
    free(text);

    for (int i = 0; i < 4; ++i)
        CHECK(ccr_editor_undo(&ed), "undo %d failed", i);
    text = generate(&ed);
    CHECK(text && strcmp(text, ed.ast.source) == 0, "undoing the comments did not restore the file");
    free(text);
    ccr_editor_free(&ed);
}

/* ------------------------------------------------- expressions in slots */

/* The n-th expression (not a statement) whose own text is exactly `text`. */
static ccr_node_id expr_node(const ccr_editor *ed, const char *text, int nth)
{
    size_t n = strlen(text);
    for (ccr_node_id i = 0; i < ed->ast.node_count; ++i) {
        const ccr_node *k = N(ed, i);
        if (k->parent == CCR_NONE || N(ed, k->parent)->kind == CCR_NODE_BLOCK ||
            N(ed, k->parent)->kind == CCR_NODE_TRANSLATION_UNIT || (k->flags & CCR_FLAG_DEAD))
            continue;
        if (k->src_e - k->src_b == n && memcmp(ed->ast.source + k->src_b, text, n) == 0 &&
            nth-- == 0)
            return i;
    }
    fprintf(stderr, "no expression \"%s\"\n", text);
    ++g_failures;
    return CCR_NONE;
}

/* The empty slot an expression left behind: its parent's child at `index`. */
static ccr_node_id hole_in(const ccr_editor *ed, ccr_node_id parent, int index)
{
    ccr_node_id h = kid(ed, parent, index);
    CHECK(h != CCR_NONE && N(ed, h)->kind == CCR_NODE_TEXT &&
          !*ccr_ast_str(&ed->ast, N(ed, h)->name), "no empty slot where expected");
    return h;
}

static void put_in_slot(ccr_editor *ed, ccr_node_id x, ccr_node_id hole)
{
    ccr_vec2 at;
    if (snap_at(ed, CCR_SNAP_SLOT, hole, &at))
        drag(ed, corner(ed, x), at);
}

/* Scratch: grab a reporter and it comes out of its slot, leaving it empty. */
static void test_expression_swap(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id three = expr_node(&ed, "3", 0);   /* if (n > 3) */
    ccr_node_id ten = expr_node(&ed, "10", 0);    /* while (b < 10) */
    ccr_node_id gt = N(&ed, three)->parent, lt = N(&ed, ten)->parent;

    drag(&ed, corner(&ed, three), far_away(0));
    CHECK(N(&ed, three)->parent != gt, "3 did not come out of n > 3");
    CHECK(ccr_editor_loose_count(&ed) == 1, "3 is not lying on the canvas");
    ccr_node_id hole1 = hole_in(&ed, gt, 1);

    char err[256];
    char *text = save_to(&ed, "expr_hole.c", err, sizeof err);
    CHECK(!text && strstr(err, "empty slot"), "a program with a hole was saved: %s", err);
    free(text);

    put_in_slot(&ed, ten, hole1);                  /* n > 10, b < _ */
    CHECK(N(&ed, ten)->parent == gt, "10 did not go into the slot");
    ccr_node_id hole2 = hole_in(&ed, lt, 1);
    put_in_slot(&ed, three, hole2);                /* b < 3 */
    CHECK(N(&ed, three)->parent == lt && ccr_editor_loose_count(&ed) == 0,
          "3 did not go into the other slot");

    text = save_to(&ed, "expr_swap.c", err, sizeof err);
    CHECK(text && strstr(text, "if (n > 10)\n        n--;") && strstr(text, "while (b < 3) {"),
          "swapped expressions not saved: %s", text ? "" : err);
    CHECK(text && strstr(text, "// braceless then") && strstr(text, "// nothing else here"),
          "comments around the slots lost");
    free(text);
    check_compiles("expr_swap.c");

    for (int i = 0; i < 3; ++i)
        CHECK(ccr_editor_undo(&ed), "undo %d failed", i);
    text = generate(&ed);
    CHECK(text && strcmp(text, ed.ast.source) == 0, "undoing the swaps did not restore the file");
    free(text);
    CHECK(ccr_editor_redo(&ed) && ccr_editor_redo(&ed) && ccr_editor_redo(&ed), "redo failed");
    text = generate(&ed);
    CHECK(text && strstr(text, "while (b < 3) {"), "redo lost the swap");
    free(text);
    ccr_editor_free(&ed);
}

/* Code made by a macro is grabbed whole: TWICE(n), never the n inside. */
static void test_expression_macro(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id twice = expr_node(&ed, "TWICE(n)", 0);
    ccr_node_id two = kid(&ed, twice, 1);          /* the 2 inside ((x) * 2) */
    ccr_vec2 c = corner(&ed, two);
    ccr_context ctx = ccr_editor_context(&ed, c);
    CHECK(ctx.target == twice, "grabbing inside a macro did not take the whole macro "
          "(hit %u %s, target %u %s, twice %u)", ctx.node,
          ctx.node != CCR_NONE ? ccr_node_kind_name(N(&ed, ctx.node)->kind) : "-", ctx.target,
          ctx.target != CCR_NONE ? ccr_node_kind_name(N(&ed, ctx.target)->kind) : "-", twice);

    CHECK(ccr_editor_delete(&ed, ctx.target), "delete of TWICE(n) refused");
    char err[256];
    char *text = save_to(&ed, "expr_macro.c", err, sizeof err);
    CHECK(!text, "saved with an empty slot");
    free(text);
    ccr_node_id hole = hole_in(&ed, N(&ed, expr_node(&ed, "counter", 0))->parent, 1);
    CHECK(ccr_editor_set_text(&ed, hole, "TWICE(n) + 1"), "typing into the slot failed");
    text = save_to(&ed, "expr_macro.c", err, sizeof err);
    CHECK(text && strstr(text, "    counter += TWICE(n) + 1;\n"), "typed slot not saved: %s",
          text ? "" : err);
    free(text);
    check_compiles("expr_macro.c");
    CHECK(ccr_editor_undo(&ed) && ccr_editor_undo(&ed), "undo failed");
    text = generate(&ed);
    CHECK(text && strcmp(text, ed.ast.source) == 0, "undo did not restore the macro");
    free(text);
    ccr_editor_free(&ed);
}

/* Duplicate a reporter into a slot; drop one into a palette block. */
static void test_expression_copy_and_palette(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");

    /* if from the palette, after `int a = 1, b = 2;` */
    ccr_node_id hello = stmt(&ed, "int a = 1");
    ccr_vec2 after;
    ccr_node_id fresh = spawn_at(&ed, CCR_TEMPLATE_IF, far_away(0));
    if (snap_at(&ed, CCR_SNAP_AFTER, hello, &after))
        drag(&ed, corner(&ed, fresh), after);
    CHECK(N(&ed, fresh)->parent == N(&ed, hello)->parent, "the if did not snap in");

    /* the while's condition goes into the if; a copy of it back into the while */
    ccr_node_id cond = expr_node(&ed, "b < 10", 0);
    ccr_node_id loop = N(&ed, cond)->parent;
    put_in_slot(&ed, cond, kid(&ed, fresh, 0));
    CHECK(N(&ed, cond)->parent == fresh, "the condition did not go into the palette if");

    CHECK(ccr_editor_duplicate(&ed, cond, corner(&ed, cond)), "duplicate refused");
    ccr_vec2 at;
    if (snap_at(&ed, CCR_SNAP_SLOT, hole_in(&ed, loop, 0), &at)) {
        frame(&ed, (ccr_vec2){ at.x + 10.0f, at.y + 10.0f }, false, false, false);
        frame(&ed, (ccr_vec2){ at.x + 10.0f, at.y + 10.0f }, true, true, false);
        frame(&ed, (ccr_vec2){ at.x + 10.0f, at.y + 10.0f }, false, false, true);
    }
    CHECK(ed.drag == CCR_DRAG_IDLE && ccr_editor_loose_count(&ed) == 0, "the copy did not land");

    char err[256];
    char *text = save_to(&ed, "expr_copy.c", err, sizeof err);
    CHECK(text && strstr(text, "if (b < 10) {") && strstr(text, "while (b < 10) {"),
          "copied condition not saved: %s", text ? "" : err);
    free(text);
    check_compiles("expr_copy.c");
    ccr_editor_free(&ed);
}

/* Things that are not reporters do not come out: a declaration's name,
 * a statement, an initializer whose parent is not plain text. */
static void test_expression_not_movable(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id bump = stmt(&ed, "counter += TWICE(n)");
    ccr_context ctx = ccr_editor_context(&ed, corner(&ed, bump));
    CHECK(ctx.target == bump, "the statement's own corner grabbed an expression");

    /* `{ a, b }` lives in `struct pair {...} pr = ...`, whose struct is not
     * inside the variable's text: not safe to rewrite, so it stays */
    ccr_node_id a = expr_node(&ed, "a", 0);
    ccr_node_id pr_a = CCR_NONE;
    for (int i = 0; i < 8 && a != CCR_NONE; ++i, a = expr_node(&ed, "a", i))
        if (N(&ed, N(&ed, a)->parent)->kind == CCR_NODE_INIT_LIST) {
            pr_a = a;
            break;
        }
    if (pr_a != CCR_NONE) {
        ctx = ccr_editor_context(&ed, corner(&ed, pr_a));
        CHECK(ctx.target != pr_a, "an expression in an unsafe place came out");
    }
    ccr_editor_free(&ed);
}

/* A click on an operator swaps it for another of its kind; the new one
 * is printed around the operands' own text, with parentheses if needed. */
static void test_swap_operator(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id gt = expr_node(&ed, "n > 3", 0);
    CHECK(ccr_editor_set_op(&ed, gt, ">="), "> to >= refused");
    CHECK(!ccr_editor_set_op(&ed, gt, "+"), "a comparison became a sum");

    /* return a + b + pr.x + pr.y  is  ((a + b) + pr.x) + pr.y */
    ccr_node_id inner = expr_node(&ed, "a + b", 0);
    CHECK(ccr_editor_set_op(&ed, inner, "*"), "+ to * refused");
    ccr_node_id outer = expr_node(&ed, "a + b + pr.x + pr.y", 0);
    CHECK(ccr_editor_set_op(&ed, outer, "*"), "outer + to * refused");

    ccr_node_id inc = stmt(&ed, "n++");
    CHECK(ccr_editor_set_op(&ed, inc, "--"), "++ to -- refused");

    /* the * of TWICE(x) lives in the macro: not offered */
    for (ccr_node_id i = 0; i < ed.ast.node_count; ++i)
        if (N(&ed, i)->kind == CCR_NODE_BINARY_OP &&
            strcmp(ccr_ast_str(&ed.ast, N(&ed, i)->op), "*") == 0 && !(N(&ed, i)->flags & CCR_FLAG_NEW))
            CHECK(!ccr_codegen_can_reprint(&ed.ast, i), "an operator inside a macro is offered");

    char err[256];
    char *text = save_to(&ed, "swap_op.c", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        CHECK(strstr(text, "if (n >= 3)\n"), "n >= 3 not saved");
        CHECK(strstr(text, "return (a * b + pr.x) * pr.y;"), "parentheses wrong:\n%s", text);
        CHECK(strstr(text, "        n--;\n"), "n-- not saved");
        free(text);
        check_compiles("swap_op.c");
    }
    while (ccr_editor_undo(&ed))
        ;
    char *back = generate(&ed);
    CHECK(back && strcmp(back, ed.ast.source) == 0, "undoing the swaps did not restore the file");
    free(back);
    ccr_editor_free(&ed);
}

static uint32_t script_of(const ccr_editor *ed, ccr_node_id root)
{
    for (uint32_t i = 0; i < ed->script_count; ++i)
        if (ed->scripts[i].root == root)
            return i;
    return 0;
}

/* A declaration from the palette, let go anywhere on the canvas. */
static ccr_node_id declare(ccr_editor *ed, ccr_template t, float x)
{
    const ccr_vec2 where = { x, -3000.0f };
    CHECK(ccr_editor_spawn(ed, t, where), "%s: spawn refused", ccr_template_label(t));
    ccr_node_id loose = ed->scripts[ed->script_count - 1].root;
    ccr_node_id made = N(ed, loose)->first_child;
    frame(ed, where, false, false, true);
    CHECK(N(ed, made)->parent == 0, "%s did not join the file", ccr_template_label(t));
    CHECK(ccr_editor_loose_count(ed) == 0, "%s left something loose", ccr_template_label(t));
    return made;
}

/* The file's own declarations from the palette: each goes where C wants
 * it, whatever order they are made in. */
static void test_new_declarations(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");

    ccr_node_id fn = declare(&ed, CCR_TEMPLATE_FUNC_DEF, 6000.0f);
    type(&ed, word(&ed, fn, CCR_FLAG_WORD_NAME), "triple");
    type(&ed, slot_of(&ed, fn, 0), "int x");
    ccr_vec2 at = { 0, 0 };
    snap_at(&ed, CCR_SNAP_FIRST, slot_of(&ed, fn, 1), &at);
    type(&ed, slot_of(&ed, spawn_at(&ed, CCR_TEMPLATE_RETURN, at), 0), "x * 3");

    ccr_node_id proto = declare(&ed, CCR_TEMPLATE_FUNC_DECL, 6600.0f);
    type(&ed, word(&ed, proto, CCR_FLAG_WORD_NAME), "triple");
    type(&ed, slot_of(&ed, proto, 0), "int x");

    ccr_node_id st = declare(&ed, CCR_TEMPLATE_STRUCT_DEF, 7200.0f);
    snap_at(&ed, CCR_SNAP_FIRST, slot_of(&ed, st, 0), &at);
    ccr_node_id fx = spawn_at(&ed, CCR_TEMPLATE_DECL, at);
    snap_at(&ed, CCR_SNAP_AFTER, fx, &at);
    type(&ed, word(&ed, spawn_at(&ed, CCR_TEMPLATE_DECL, at), CCR_FLAG_WORD_NAME), "y");

    declare(&ed, CCR_TEMPLATE_TYPEDEF, 7800.0f);
    declare(&ed, CCR_TEMPLATE_ENUM_DEF, 8400.0f);
    declare(&ed, CCR_TEMPLATE_GLOBAL_VAR, 9000.0f);
    declare(&ed, CCR_TEMPLATE_INCLUDE_SYS, 9600.0f);
    type(&ed, word(&ed, declare(&ed, CCR_TEMPLATE_INCLUDE_SYS, 10200.0f), CCR_FLAG_WORD_NAME), "stdlib.h");
    declare(&ed, CCR_TEMPLATE_DEFINE, 10800.0f);
    /* one made and thrown away again: gone from the file */
    ccr_node_id gone = declare(&ed, CCR_TEMPLATE_DEFINE, 11400.0f);
    CHECK(ccr_editor_delete(&ed, gone), "a new declaration cannot be deleted");

    char err[256];
    char *text = save_to(&ed, "declarations.c", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        CHECK(strncmp(text, "/* save.c: everything saving must keep intact */\n"
                            "#include <stdio.h>\n#include <stdio.h>\n#include <stdlib.h>\n\n"
                            "#define TWICE(x) ((x) * 2)\n#define SIZE 64\n", 150) == 0,
              "the top of the file:\n%.300s", text);
        check_order(text, "#define SIZE 64", "struct point {\n    int x;\n    int y;\n};");
        check_order(text, "struct point {", "typedef struct point point;");
        check_order(text, "typedef struct point point;", "enum mood { CALM, ANGRY };");
        check_order(text, "enum mood", "int triple(int x);");
        check_order(text, "int triple(int x);", "static int counter = 0;   // file-level comment");
        check_order(text, "static int counter = 0;   // file-level", "static int counter = 0;\n");
        check_order(text, "static int counter = 0;\n", "int work(int n)");
        check_order(text, "    return 0;\n}\n", "\nint triple(int x)\n{\n    return x * 3;\n}\n");
        CHECK(count(text, "#define SIZE") == 1, "the deleted #define is still there");
        free(text);
    }
    /* counter twice does not compile; the rest is checked by the order */
    while (ccr_editor_undo(&ed))
        ;
    char *back = generate(&ed);
    CHECK(back && strcmp(back, ed.ast.source) == 0, "undoing the declarations did not restore the file");
    free(back);
    ccr_editor_free(&ed);
}

/* A header: new declarations go inside its guard, before the #endif. */
static void test_header_guard(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/guarded.h", g_outdir);
    FILE *f = fopen(path, "w");
    if (!f) {
        CHECK(false, "cannot write %s", path);
        return;
    }
    fputs("#ifndef GUARDED_H\n#define GUARDED_H\n\n#endif\n", f);
    fclose(f);

    ccr_ast ast;
    ccr_ast_init(&ast);
    CHECK(ccr_parse_file(path, NULL, 0, &ast), "the header did not parse");
    ccr_editor ed;
    CHECK(ccr_editor_init(&ed, &ast), "editor init failed");
    ccr_node_id proto = declare(&ed, CCR_TEMPLATE_FUNC_DECL, 6000.0f);
    type(&ed, word(&ed, proto, CCR_FLAG_WORD_NAME), "twice");
    type(&ed, slot_of(&ed, proto, 0), "int x");
    declare(&ed, CCR_TEMPLATE_STRUCT_DEF, 6600.0f);
    declare(&ed, CCR_TEMPLATE_INCLUDE_SYS, 7200.0f);

    char err[256];
    char *text = save_to(&ed, "guarded_out.h", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        CHECK(strcmp(text, "#ifndef GUARDED_H\n#define GUARDED_H\n#include <stdio.h>\n\n"
                           "struct point {\n};\n\nint twice(int x);\n\n#endif\n") == 0,
              "the header came out as:\n%s", text);
        free(text);
        check_compiles("guarded_out.h");
    }
    ccr_editor_free(&ed);
}

/* Folding is a view: the block gets short, the file stays the same, and
 * undo neither folds nor unfolds. */
static void test_fold(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id loop = stmt(&ed, "while (b < 10)");
    ccr_node_id fn = N(&ed, N(&ed, loop)->parent)->parent;   /* work() */
    CHECK(ccr_editor_can_fold(&ed, loop) && ccr_editor_can_fold(&ed, fn), "while/function do not fold");
    CHECK(!ccr_editor_can_fold(&ed, stmt(&ed, "printf(\"hello")), "a call folds");

    ccr_rect open_r, shut_r;
    ccr_editor_node_rect(&ed, loop, &open_r);
    const ccr_rect before = ed.scripts[script_of(&ed, fn)].blocks.bounds;
    CHECK(ccr_editor_toggle_fold(&ed, loop), "fold refused");
    ccr_editor_node_rect(&ed, loop, &shut_r);
    const ccr_rect after = ed.scripts[script_of(&ed, fn)].blocks.bounds;
    CHECK(after.h < before.h, "folding the while did not make main shorter (%.0f -> %.0f)", before.h, after.h);
    ccr_rect hidden;
    CHECK(!ccr_editor_node_rect(&ed, stmt(&ed, "b++"), &hidden), "a folded statement is still drawn");
    CHECK(ed.history_len == 0, "folding made an undo step");

    /* an edit and its undo leave the fold alone */
    lift_one(&ed, stmt(&ed, "do b--;"), 0);
    ccr_editor_undo(&ed);
    while (ccr_editor_undo(&ed))
        ;
    CHECK(N(&ed, loop)->flags & CCR_FLAG_COLLAPSED, "undo unfolded the while");
    char *text = generate(&ed);
    CHECK(text && strcmp(text, ed.ast.source) == 0, "a fold changed the file");
    free(text);

    /* a whole function folds to its hat */
    CHECK(ccr_editor_toggle_fold(&ed, fn), "function fold refused");
    CHECK(ed.scripts[script_of(&ed, fn)].blocks.bounds.h < 90.0f, "a folded function is %.0f tall",
          ed.scripts[script_of(&ed, fn)].blocks.bounds.h);
    /* reveal opens what hides a statement */
    ccr_editor_reveal(&ed, stmt(&ed, "b++"));
    CHECK(ccr_editor_node_rect(&ed, stmt(&ed, "b++"), &hidden), "reveal did not show b++");
    CHECK(!(N(&ed, fn)->flags & CCR_FLAG_COLLAPSED) && !(N(&ed, loop)->flags & CCR_FLAG_COLLAPSED),
          "reveal left a fold");
    ccr_editor_free(&ed);
}

/* Block packs: a library call from the palette is the plain C call when
 * saved; a call in the file with a pack's name and arity is drawn as the
 * pack's block. */
/* Code from the clipboard, as blocks; and back to the clipboard. */
static ccr_snippet g_snip;   /* big: off the stack */
static const ccr_node *N2(const ccr_ast *a, ccr_node_id id) { return &a->nodes[id]; }

static bool read_snippet(const ccr_editor *ed, const char *code)
{
    char *ctx = generate(ed);
    const bool ok = ccr_snippet_parse(code, ctx, ctx ? strlen(ctx) : 0, g_input, false, NULL, 0, &g_snip);
    free(ctx);
    return ok;
}

/* The carried paste, dropped right after statement `after`. */
static void drop_after(ccr_editor *ed, ccr_node_id after)
{
    ccr_vec2 at;
    if (!snap_at(ed, CCR_SNAP_AFTER, after, &at))
        return;
    const ccr_vec2 hold = { at.x + 10.0f, at.y + 10.0f };
    frame(ed, hold, false, false, false);
    frame(ed, hold, true, true, false);
    frame(ed, hold, false, false, true);
}

static void test_paste(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    char *before = generate(&ed);

    /* statements the file can read */
    CHECK(read_snippet(&ed, "counter += 2;\n    printf(\"%d\\n\", counter);"), "snippet");
    CHECK(g_snip.count == 2 && g_snip.as_text == 0 && !g_snip.reporter && !g_snip.toplevel,
          "two statements: %d, %d as text", g_snip.count, g_snip.as_text);
    const ccr_node_id hello = stmt(&ed, "printf(\"hello\\n\")");
    CHECK(ccr_editor_paste(&ed, &g_snip.ast, g_snip.items, g_snip.count, false, far_away(0.0f)), "paste");
    ccr_snippet_free(&g_snip);
    drop_after(&ed, hello);
    CHECK(ed.drag == CCR_DRAG_IDLE && ccr_editor_loose_count(&ed) == 0, "the paste did not land");
    char err[256];
    char *text = save_to(&ed, "pasted.c", err, sizeof err);
    CHECK(text && strstr(text, "    printf(\"hello\\n\");\n    counter += 2;\n    printf(\"%d\\n\", counter);\n"),
          "pasted statements saved as:\n%s", text ? text : err);
    free(text);
    check_compiles("pasted.c");

    /* and copied back out: the same code */
    const ccr_node_id added = N(&ed, hello)->next_sibling;
    const ccr_node_id two[2] = { N(&ed, added)->next_sibling, added };   /* out of order on purpose */
    char *copied = ccr_copy_text(&ed.ast, two, 2, err, sizeof err);
    CHECK(copied && strcmp(copied, "counter += 2;\nprintf(\"%d\\n\", counter);") == 0, "copied: [%s]",
          copied ? copied : "(null)");
    free(copied);

    /* never saved: a stack left loose prints itself */
    CHECK(read_snippet(&ed, "if (counter > 1) {\n        counter = 0;\n    }\n    counter++;"), "snippet loose");
    CHECK(ccr_editor_paste(&ed, &g_snip.ast, g_snip.items, g_snip.count, false, far_away(0.0f)), "paste loose");
    ccr_snippet_free(&g_snip);
    frame(&ed, far_away(0.0f), false, false, false);
    frame(&ed, far_away(0.0f), true, true, false);
    frame(&ed, far_away(0.0f), false, false, true);
    const ccr_node_id lroot = ed.scripts[ed.script_count - 1].root;
    CHECK(ccr_editor_loose_count(&ed) == 1, "the paste should lie loose");
    copied = ccr_copy_text(&ed.ast, &lroot, 1, err, sizeof err);
    CHECK(copied && strcmp(copied, "if (counter > 1) {\n    counter = 0;\n}\ncounter++;") == 0,
          "copied loose: [%s] %s", copied ? copied : "(null)", err);
    free(copied);
    const ccr_vec2 apart = far_away(3000.0f);
    CHECK(ccr_editor_spawn(&ed, CCR_TEMPLATE_BREAK, apart), "spawn break");
    frame(&ed, apart, false, false, true);
    const ccr_node_id broot = ed.scripts[ed.script_count - 1].root;
    copied = ccr_copy_text(&ed.ast, &broot, 1, err, sizeof err);
    CHECK(copied && strcmp(copied, "break;") == 0, "copied new break: [%s] %s", copied ? copied : "(null)", err);
    free(copied);
    const ccr_vec2 apart2 = far_away(6000.0f);
    CHECK(ccr_editor_spawn(&ed, CCR_TEMPLATE_WHILE, apart2), "spawn while");
    frame(&ed, apart2, false, false, true);
    const ccr_node_id wroot = ed.scripts[ed.script_count - 1].root;
    copied = ccr_copy_text(&ed.ast, &wroot, 1, err, sizeof err);
    CHECK(!copied && err[0], "an empty slot: no copy, and why (%s)", err);
    free(copied);
    const ccr_node_id both[2] = { broot, lroot };
    copied = ccr_copy_text(&ed.ast, both, 2, err, sizeof err);
    CHECK(copied && strcmp(copied, "break;\nif (counter > 1) {\n    counter = 0;\n}\ncounter++;") == 0,
          "two loose stacks: [%s]", copied ? copied : "(null)");
    free(copied);

    /* break and continue read in a loop, as blocks */
    CHECK(read_snippet(&ed, "if (counter) break;\ncontinue;") && g_snip.count == 2 && g_snip.as_text == 0 &&
          N2(&g_snip.ast, g_snip.items[0])->kind == CCR_NODE_IF, "break/continue: %d items, %d as text",
          g_snip.count, g_snip.as_text);
    ccr_snippet_free(&g_snip);

    /* a variable this file does not have: kept as a code line, not lost */
    CHECK(read_snippet(&ed, "nowhere_x = 3;\nputs(\"ok\");"), "snippet 2");
    CHECK(g_snip.count == 2 && g_snip.as_text == 1, "unknown name: %d items, %d as text", g_snip.count, g_snip.as_text);
    ccr_snippet_free(&g_snip);

    /* one expression: a reporter */
    CHECK(read_snippet(&ed, "counter * 2") && g_snip.reporter && g_snip.count == 1, "an expression");
    ccr_snippet_free(&g_snip);

    /* a whole function: into the file, where functions go */
    CHECK(read_snippet(&ed, "static int twice(int v)\n{\n    return v * 2;\n}\n") && g_snip.toplevel &&
          g_snip.count == 1, "a function: toplevel %d, %d items", g_snip.toplevel, g_snip.count);
    CHECK(ccr_editor_paste_decls(&ed, &g_snip.ast, g_snip.items, g_snip.count, far_away(0.0f)), "paste decls");
    ccr_snippet_free(&g_snip);
    text = save_to(&ed, "pasted_fn.c", err, sizeof err);
    CHECK(text && strstr(text, "static int twice(int v)\n{\n    return v * 2;\n}\n"), "pasted function:\n%s",
          text ? text : err);
    free(text);
    check_compiles("pasted_fn.c");

    /* undone: the file as it was */
    while (ccr_editor_undo(&ed)) {}
    char *after = generate(&ed);
    CHECK(before && after && strcmp(before, after) == 0, "undo left a paste behind");
    free(before);
    free(after);
    ccr_editor_free(&ed);
}

/* Find: words on the canvas, any case; folded ones too. */
static void test_find(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_find_hit hits[64];
    const uint32_t n = ccr_editor_find(&ed, "HELLO", hits, 64);
    CHECK(n >= 1, "\"hello\" not found");
    for (uint32_t i = 1; i < n && i < 64; ++i)
        CHECK(hits[i].rect.y >= hits[i - 1].rect.y, "hits not top to bottom");
    CHECK(ccr_editor_find(&ed, "no such word anywhere", hits, 64) == 0, "found nothing as something");
    /* fold the function holding it: still found, as hidden */
    const ccr_node_id hello = n ? hits[0].node : CCR_NONE;
    ccr_node_id fn = hello;
    while (fn != CCR_NONE && N(&ed, fn)->kind != CCR_NODE_FUNCTION)
        fn = N(&ed, fn)->parent;
    CHECK(fn != CCR_NONE && ccr_editor_toggle_fold(&ed, fn), "fold");
    const uint32_t m = ccr_editor_find(&ed, "hello", hits, 64);
    bool hidden = false;
    for (uint32_t i = 0; i < m && i < 64; ++i)
        hidden = hidden || hits[i].hidden;
    CHECK(m >= 1 && hidden, "folded away: %u hits, hidden %d", m, hidden);
    ccr_editor_free(&ed);
}

static void test_library_blocks(void)
{
    ccr_lib_load(stderr);
    const ccr_lib_block *dot = ccr_lib_find("draw_dot", 2);
    CHECK(dot != NULL, "the toy pack is not loaded (CCRATCH_BLOCKS_DIR?)");
    if (!dot)
        return;
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_node_id made = add_after(&ed, CCR_TEMPLATE_LIB(ccr_lib_index(dot)), stmt(&ed, "do b--;"));
    CHECK(N(&ed, made)->kind == CCR_NODE_CALL, "a pack block is a call");
    type(&ed, slot_of(&ed, made, 1), "n * 2");

    /* drawn with the pack's words, not as draw_dot(...) */
    const ccr_blocks *bl = &ed.scripts[script_of(&ed, N(&ed, N(&ed, made)->parent)->parent)].blocks;
    bool words = false, raw = false;
    for (uint32_t i = 0; i < bl->prim_count; ++i)
        if (bl->prims[i].kind == CCR_PRIM_TEXT && bl->prims[i].node == made) {
            words = words || strcmp(bl->text + bl->prims[i].text, "dot at") == 0;
            raw = raw || strstr(bl->text + bl->prims[i].text, "draw_dot") != NULL;
        }
    CHECK(words && !raw, "the block does not read 'dot at ...'");

    /* a reporter that returns bool is pointy and goes into an if */
    const ccr_lib_block *rd = ccr_lib_find("is_ready", 0);
    ccr_node_id iff = add_after(&ed, CCR_TEMPLATE_IF, made);
    fill(&ed, CCR_TEMPLATE_LIB(ccr_lib_index(rd)), slot_of(&ed, iff, 0));

    char err[256];
    char *text = save_to(&ed, "library.c", err, sizeof err);
    CHECK(text != NULL, "save failed: %s", err);
    if (text) {
        CHECK(strstr(text, "    draw_dot(1, n * 2);\n    if (is_ready()) {\n    }\n"),
              "pack blocks saved as:\n%s", text);
        /* the first block of the pack brought its header, once */
        CHECK(strstr(text, "#include <stdio.h>\n#include \"toy.h\"\n") && count(text, "toy.h") == 1,
              "the pack's header is not included once:\n%s", text);
        free(text);
    }

    /* where each block went in the saved text: new, rebuilt, untouched */
    {
        ccr_out_range *map = calloc(ed.ast.node_count, sizeof *map);
        char *out = NULL, err2[256];
        size_t olen = 0;
        CHECK(map && ccr_codegen_mapped(&ed.ast, &out, &olen, map, err2, sizeof err2), "mapped: %s", err2);
        if (map && out) {
            const ccr_out_range m = map[made];
            CHECK(m.b != UINT32_MAX && strncmp(out + m.b, "draw_dot(1, n * 2);", m.e - m.b) == 0 &&
                  m.e - m.b == strlen("draw_dot(1, n * 2);"), "a new block: [%.*s]",
                  m.b == UINT32_MAX ? 0 : (int)(m.e - m.b), m.b == UINT32_MAX ? "" : out + m.b);
            /* printf("hello\n") was never touched: found by its parent's text */
            ccr_node_id hello = CCR_NONE;
            for (uint32_t i = 0; i < ed.ast.node_count; ++i)
                if (N(&ed, i)->kind == CCR_NODE_STRING_LIT &&
                    strcmp(ccr_ast_str(&ed.ast, N(&ed, i)->name), "\"hello\\n\"") == 0)
                    hello = i;
            CHECK(hello != CCR_NONE && map[hello].b != UINT32_MAX &&
                  strncmp(out + map[hello].b, "\"hello\\n\"", 9) == 0, "an untouched one");
            const ccr_out_range f = map[N(&ed, N(&ed, made)->parent)->parent];   /* its function */
            CHECK(f.b != UINT32_MAX && strncmp(out + f.b, "int work(int n)", 15) == 0, "its function: [%.20s]",
                  f.b == UINT32_MAX ? "" : out + f.b);
        }
        free(out);
        free(map);
    }

    /* asked for: an include, where includes go; not twice */
    CHECK(ccr_editor_add_include(&ed, "<math.h>"), "add_include");
    CHECK(!ccr_editor_add_include(&ed, "<math.h>"), "add_include twice");
    text = generate(&ed);
    CHECK(text && strstr(text, "#include \"toy.h\"\n#include <math.h>\n"), "math.h:\n%s", text ? text : "");
    free(text);

    /* undone: the include goes with the block that brought it */
    for (int i = 0; i < 5; ++i)
        ccr_editor_undo(&ed);
    text = generate(&ed);
    CHECK(text && !strstr(text, "toy.h") && !strstr(text, "draw_dot") && !strstr(text, "math.h"),
          "undo left the library behind:\n%s", text ? text : "");
    free(text);
    ccr_editor_redo(&ed);
    text = generate(&ed);
    CHECK(text && strstr(text, "#include \"toy.h\"\n") && strstr(text, "draw_dot(1, 2);"),
          "redo:\n%s", text ? text : "");
    free(text);
    ccr_editor_free(&ed);
    ccr_lib_unload();
}

/* A slot with a list: a value read from the file swapped for another. */
static void test_pick_value(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s", g_input);
    char *slash = strrchr(path, '/');
    snprintf(slash ? slash + 1 : path, sizeof path - (size_t)(slash ? slash + 1 - path : 0), "enums.c");
    ccr_ast ast;
    ccr_ast_init(&ast);
    ccr_editor ed;
    CHECK(ccr_parse_file(path, NULL, 0, &ast) && ccr_editor_init(&ed, &ast), "enums.c");
    char *before = generate(&ed);
    ccr_node_id green = CCR_NONE, five = CCR_NONE;
    for (ccr_node_id i = 0; i < ed.ast.node_count; ++i) {
        if (N(&ed, i)->kind == CCR_NODE_REF && strcmp(ccr_ast_str(&ed.ast, N(&ed, i)->name), "GREEN") == 0 &&
            N(&ed, i)->parent != CCR_NONE && N(&ed, N(&ed, i)->parent)->kind == CCR_NODE_CALL)
            green = i;
        if (N(&ed, i)->kind == CCR_NODE_INT_LIT)
            five = i;   /* return 0 in int main: no list */
    }
    CHECK(green != CCR_NONE && ccr_editor_value_slot(&ed, green), "GREEN is a slot with a list");
    CHECK(five != CCR_NONE && !ccr_editor_value_slot(&ed, five), "return 0 has no list");
    CHECK(ccr_editor_set_value(&ed, green, "RED"), "picked RED");
    char err[256];
    char *text = save_to(&ed, "picked.c", err, sizeof err);
    CHECK(text && strstr(text, "    go(d, RED, true);\n"), "picked value saved as:\n%s", text ? text : err);
    free(text);
    check_compiles("picked.c");
    while (ccr_editor_undo(&ed)) {}
    char *after = generate(&ed);
    CHECK(before && after && strcmp(before, after) == 0, "undo: GREEN again");
    free(before);
    free(after);
    ccr_editor_free(&ed);
}

/* A string of the tree interned again, while interning grows (and moves)
 * the strings: it must be read before the move, not after (ASan says). */
static void test_intern_own_string(void)
{
    ccr_ast a;
    CHECK(ccr_ast_init(&a), "init");
    ccr_str s = ccr_ast_intern(&a, "operator+=");
    for (int i = 0; i < 3000; ++i)
        s = ccr_ast_intern(&a, ccr_ast_str(&a, s) + (i == 0 ? 8 : 0));   /* "+=", then itself */
    CHECK(strcmp(ccr_ast_str(&a, s), "+=") == 0, "interned from its own strings: %s", ccr_ast_str(&a, s));
    ccr_ast_free(&a);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s SAVE.c OUTDIR\n", argv[0]);
        return 2;
    }
    g_input = argv[1];
    g_outdir = argv[2];
    if (!ccr_gui_init(1280, 800, "ccratch test")) {
        fprintf(stderr, "no window (run under xvfb-run)\n");
        return 2;
    }

    test_intern_own_string();
    test_unchanged();
    test_pick_value();
    test_move_with_comment();
    test_braceless_body();
    test_reindent();
    test_loose_is_left_out();
    test_refuses_directives();
    test_undo_everything();
    test_undo_and_save_point();
    test_palette_blocks();
    test_every_template();
    test_new_blocks();
    test_all_templates_spawn();
    test_swap_operator();
    test_new_declarations();
    test_header_guard();
    test_fold();
    test_find();
    test_paste();
    test_library_blocks();
    test_trash();
    test_delete();
    test_duplicate();
    test_comment_out();
    test_expression_swap();
    test_expression_macro();
    test_expression_copy_and_palette();
    test_expression_not_movable();

    ccr_gui_shutdown();
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("save: all checks passed\n");
    return g_failures ? 1 : 0;
}
