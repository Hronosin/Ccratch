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
    ccr_editor_input in = { m, pressed, down, released, 1.0f };
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

    ccr_gui_shutdown();
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("save: all checks passed\n");
    return g_failures ? 1 : 0;
}
