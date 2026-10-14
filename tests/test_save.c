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
#include "ccr_parse.h"

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
    ccr_editor_input in = { m, pressed, down, released, 1.0f, false };
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
    static const char *const fill[] = { "n++", "n > 0", "n > 0", "n > 0", NULL, "n > 0", "n", NULL, NULL };
    ccr_node_id after = stmt(&ed, "do b--;");
    for (int t = 0; t < CCR_TEMPLATE_COUNT; ++t) {
        if (t == CCR_TEMPLATE_BREAK || t == CCR_TEMPLATE_CONTINUE)
            continue;   /* need a loop around them; covered inside the while below */
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

/* thrown onto the palette: gone; from the code too */
static void test_trash(void)
{
    ccr_editor ed;
    CHECK(load(&ed), "load failed");
    ccr_vec2 start = far_away(0);
    ccr_editor_spawn(&ed, CCR_TEMPLATE_WHILE, start);
    ccr_editor_input in = { start, false, false, true, 1.0f, true };
    ccr_editor_update(&ed, &in);
    CHECK(ccr_editor_loose_count(&ed) == 0, "a block dropped on the palette stayed");

    ccr_node_id bump = stmt(&ed, "counter += TWICE(n)");
    lift_one(&ed, bump, 0);
    ccr_vec2 c = corner(&ed, bump);
    frame(&ed, c, true, true, false);
    frame(&ed, (ccr_vec2){ c.x + 50.0f, c.y }, false, true, false);
    ccr_editor_input drop = { { c.x + 60.0f, c.y }, false, false, true, 1.0f, true };
    ccr_editor_update(&ed, &drop);
    CHECK(ccr_editor_loose_count(&ed) == 0, "the thrown-away statement is still loose");

    char err[256];
    char *text = save_to(&ed, "trash.c", err, sizeof err);
    CHECK(text && !strstr(text, "counter += TWICE"), "the deleted statement was saved");
    free(text);
    CHECK(ccr_editor_undo(&ed) && ccr_editor_loose_count(&ed) == 1, "undo did not bring it back");
    ccr_editor_free(&ed);
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

    test_unchanged();
    test_move_with_comment();
    test_braceless_body();
    test_reindent();
    test_loose_is_left_out();
    test_refuses_directives();
    test_undo_everything();
    test_undo_and_save_point();
    test_palette_blocks();
    test_every_template();
    test_trash();

    ccr_gui_shutdown();
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("save: all checks passed\n");
    return g_failures ? 1 : 0;
}
